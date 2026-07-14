// SPDX-License-Identifier: Apache-2.0
// Diligent Engine 経由のテクスチャ
#pragma once

#include "render/IRhiTexture.h"
#include "memory/UniquePtr.h"
#include "container/Array.h"

namespace Diligent {
    struct ITexture;
    struct ITextureView;
}

namespace acs {

class DiligentDevice;

/**
 * Diligent Engine の ITexture を IRhiTexture として包むテクスチャ実装。
 *
 * @details
 * FTextureDesc を受けて 2D / 2D array / cubemap いずれかの ITexture を生成し、
 * 用途に応じた SRV / RTV / DSV のデフォルトビューを保持する。per_slice_rtv が
 * 指定された場合は array_size × mip_levels 個の slice/mip ごとの RTV を別途生成し
 * (IBL prefilter 等の slice 単位描画で使う)、これらは ITexture 所有外なので
 * デストラクタで明示 Release する。
 */
class DiligentTexture final : public IRhiTexture {
public:
    /** 空のテクスチャを構築する (GPU リソースは Init で確保)。 */
    DiligentTexture() noexcept = default;

    /** ITexture と per-slice RTV を解放する。 */
    ~DiligentTexture() noexcept override;

    /** コピー禁止 (GPU リソースを単独所有するため)。 */
    DiligentTexture(const DiligentTexture&) = delete;

    /** コピー代入も禁止。 */
    DiligentTexture& operator=(const DiligentTexture&) = delete;

    /**
     * FTextureDesc から ITexture とビューを生成する。
     *
     * @details
     * is_cubemap / array_size / mip_levels に応じて resource dim を決め、
     * is_render_target / is_depth_target / shader_visible_depth から bind flag を
     * 組み立てて CreateTexture する。array_size==1 かつ mips==1 の非 RT・非 depth
     * テクスチャに限り initial_data をアップロードする。per_slice_rtv 指定時は
     * slice/mip ごとの RTV も生成する。
     * @param device テクスチャ生成に使う Diligent デバイス。
     * @param desc 生成するテクスチャの記述子。
     * @return 成功なら空の TResult、検証失敗・生成失敗ならエラー。
     */
    TResult<void> Init(DiligentDevice& device, const FTextureDesc& desc) noexcept;

    /**
     * テクスチャの幅 (テクセル数) を返す。
     *
     * @return Init で渡された幅。
     */
    u32    Width()       const noexcept override { return m_Width; }

    /**
     * テクスチャの高さ (テクセル数) を返す。
     *
     * @return Init で渡された高さ。
     */
    u32    Height()      const noexcept override { return m_Height; }

    /**
     * ピクセルフォーマットを返す。
     *
     * @return テクスチャの EFormat。
     */
    EFormat EPixelFormat() const noexcept override { return m_Format; }

    /**
     * mip レベル数を返す。
     *
     * @return mip レベル数 (最小 1)。
     */
    u32    MipLevels()   const noexcept override { return m_Mips; }

    /**
     * 配列レイヤ数を返す。
     *
     * @return 配列要素数 (cubemap なら 6、最小 1)。
     */
    u32    ArraySize()   const noexcept override { return m_ArraySize; }

    /**
     * cubemap かどうかを返す。
     *
     * @return cubemap なら true。
     */
    bool   IsCubemap()   const noexcept override { return m_IsCubemap; }

    /**
     * Diligent ネイティブの ITexture を返す。
     *
     * @return 内部 ITexture (未初期化なら nullptr)。
     */
    Diligent::ITexture*     Native()    const noexcept { return m_Texture; }

    /**
     * シェーダリソースビュー (SRV) を返す。
     *
     * @return デフォルト SRV (BIND_SHADER_RESOURCE 無しなら nullptr)。
     */
    Diligent::ITextureView* SrvView()   const noexcept { return m_Srv; }

    /**
     * レンダーターゲットビュー (RTV) を返す。
     *
     * @return デフォルト RTV (RT でなければ nullptr)。
     */
    Diligent::ITextureView* RtvView()   const noexcept { return m_Rtv; }

    /**
     * 深度ステンシルビュー (DSV) を返す。
     *
     * @return デフォルト DSV (depth target でなければ nullptr)。
     */
    Diligent::ITextureView* DsvView()   const noexcept { return m_Dsv; }

    /**
     * 順序なしアクセスビュー (UAV) を返す (Phase 0、compute の RWTexture 用)。
     *
     * @return デフォルト UAV (is_uav=false なら nullptr)。
     */
    Diligent::ITextureView* UavView()   const noexcept { return m_Uav; }

    /**
     * per_slice_rtv で生成した slice/mip ごとの RTV を返す。
     *
     * @param slice 配列スライス (cubemap face) のインデックス。
     * @param mip mip レベルのインデックス。
     * @return 該当する RTV (範囲外・未生成なら nullptr)。
     */
    Diligent::ITextureView* RtvSlice(u32 slice, u32 mip) const noexcept;

    /**
     * レンダーターゲットとして生成されたかを返す。
     *
     * @return RT なら true。
     */
    bool IsRenderTarget()    const noexcept { return m_IsRt; }

    /**
     * 深度ターゲットとして生成されたかを返す。
     *
     * @return depth target なら true。
     */
    bool IsDepthTarget()     const noexcept { return m_IsDepth; }

    /**
     * 深度バッファが stencil 成分を持つかを返す。
     *
     * @details D24_UNorm_S8_UInt の depth target のときに true。clear 時に stencil
     * フラグを足すかの判定に使う。
     * @return stencil 付き depth なら true。
     */
    bool HasStencil()        const noexcept {
        return m_IsDepth && m_Format == EFormat::D24_UNorm_S8_UInt;
    }

    /**
     * 深度テクスチャがシェーダから読めるかを返す。
     *
     * @return shader_visible_depth で生成されていれば true。
     */
    bool ShaderVisibleDepth() const noexcept { return m_DepthSrv; }

private:
    /** per-slice view とテクスチャ本体を解放して空状態へ戻す。 */
    void Reset() noexcept;

    /** Init で受け取った所有元デバイス。 */
    DiligentDevice*         m_Device  = nullptr;

    /** Diligent ネイティブのテクスチャ本体。 */
    Diligent::ITexture*     m_Texture = nullptr;

    /** シェーダリソースビュー (m_Texture 所有のため addref しない)。 */
    Diligent::ITextureView* m_Srv     = nullptr;

    /** レンダーターゲットビュー (m_Texture 所有)。 */
    Diligent::ITextureView* m_Rtv     = nullptr;

    /** 深度ステンシルビュー (m_Texture 所有)。 */
    Diligent::ITextureView* m_Dsv     = nullptr;

    /** 順序なしアクセスビュー (UAV、m_Texture 所有、Phase 0)。 */
    Diligent::ITextureView* m_Uav     = nullptr;

    /** UAV (RWTexture) として生成されたか。 */
    bool                    m_IsUav   = false;

    /** 3D テクスチャの奥行き (1 = 2D)。 */
    u32                     m_Depth   = 1;

    /**
     * per_slice_rtv 指定時に生成する slice/mip ごとの RTV 群。
     *
     * @details
     * array_size × mip_levels 個を Init で CreateView して保持する。これらは
     * m_Texture 所有のデフォルトビューと異なり別途生成されるため、デストラクタで
     * 明示 Release が必要。
     */
    TArray<Diligent::ITextureView*> m_SliceRtvs;

    /** テクスチャの幅 (テクセル数)。 */
    u32                     m_Width   = 0;

    /** テクスチャの高さ (テクセル数)。 */
    u32                     m_Height  = 0;

    /** mip レベル数 (最小 1)。 */
    u32                     m_Mips    = 1;

    /** 配列レイヤ数 (cubemap なら 6、最小 1)。 */
    u32                     m_ArraySize = 1;

    /** ピクセルフォーマット。 */
    EFormat                  m_Format  = EFormat::R8G8B8A8_UNorm;

    /** レンダーターゲットとして生成したか。 */
    bool                    m_IsRt    = false;

    /** 深度ターゲットとして生成したか。 */
    bool                    m_IsDepth = false;

    /** 深度テクスチャをシェーダから読めるようにしたか。 */
    bool                    m_DepthSrv = false;

    /** cubemap として生成したか。 */
    bool                    m_IsCubemap = false;
};

} // namespace acs
