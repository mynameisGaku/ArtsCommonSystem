// SPDX-License-Identifier: Apache-2.0
// DX12 テクスチャ実装
#pragma once

#include "render/IRhiTexture.h"
#include "render/Dx12/Dx12Common.h"
#include "container/Array.h"

namespace acs {

class Dx12Device;

/**
 * IRhiTexture の DX12 実装 (2D テクスチャ / 配列 / キューブマップ / 深度・RT)。
 *
 * @details
 * DEFAULT ヒープに ID3D12Resource を確保し、用途に応じて SRV / DSV / RTV を
 * Dx12Device のディスクリプタヒープから割り当てる。配列・キューブマップ・per-slice
 * RTV・mip 連鎖に対応し、IBL の env/irradiance/prefilter cubemap (array_size=6 +
 * per_slice_rtv) でも使える。深度ターゲットは SRV/DSV を両方作れるよう TYPELESS で
 * 確保する。現在のリソース状態を保持し、CommandList のバリア発行に追従させる。
 */
class Dx12Texture final : public IRhiTexture {
public:
    /** 空状態で構築する (GPU リソースは Init で確保)。 */
    Dx12Texture() noexcept = default;

    /** SRV/DSV/RTV スロットを解放しリソースを Release する。 */
    ~Dx12Texture() noexcept override;

    /**
     * desc に従って DX12 テクスチャと必要なビュー (SRV/DSV/RTV) を生成する。
     *
     * @details
     * DEFAULT ヒープに ID3D12Resource を作り、非深度・初期データありなら UPLOAD ヒープ
     * 経由でコピーする。深度ターゲットは DSV (と shader_visible_depth=true なら SRV) を、
     * カラーは SRV を作る。is_render_target なら RTV も作り、per_slice_rtv=true のときは
     * array_size*mip_levels 個の per-slice RTV を生成する。
     * @param device リソース・ディスクリプタ確保に使う DX12 デバイス。
     * @param desc 生成するテクスチャの記述 (サイズ・フォーマット・用途フラグ・初期データ)。
     * @return 成功なら成功状態の HrResult、失敗なら HRESULT を保持したエラー。
     */
    HrResult Init(Dx12Device& device, const FTextureDesc& desc) noexcept;

    /**
     * テクスチャの幅 (ピクセル) を返す。
     *
     * @return 幅。
     */
    u32    Width()       const noexcept override { return m_Width; }

    /**
     * テクスチャの高さ (ピクセル) を返す。
     *
     * @return 高さ。
     */
    u32    Height()      const noexcept override { return m_Height; }

    /**
     * ピクセルフォーマットを返す。
     *
     * @return EFormat。
     */
    EFormat EPixelFormat() const noexcept override { return m_Format; }

    /**
     * mip レベル数を返す。
     *
     * @return mip レベル数 (最低 1)。
     */
    u32    MipLevels()   const noexcept override { return m_MipLevels; }

    /**
     * 配列スライス数を返す (キューブマップは 6 の倍数)。
     *
     * @return 配列サイズ (最低 1)。
     */
    u32    ArraySize()   const noexcept override { return m_ArraySize; }

    /**
     * キューブマップとして作成されたかを返す。
     *
     * @return キューブマップなら true。
     */
    bool   IsCubemap()   const noexcept override { return m_IsCubemap; }

    /**
     * SRV の GPU ディスクリプタハンドルを返す (ルート記述子テーブルへバインドする内部用)。
     *
     * @return SRV の GPU ハンドル (Device が無ければ null ハンドル)。
     */
    D3D12_GPU_DESCRIPTOR_HANDLE SrvGpuHandle() const noexcept;

    /**
     * DSV の CPU ディスクリプタハンドルを返す (深度バッファのみの内部用)。
     *
     * @return DSV の CPU ハンドル (Device が無ければ null ハンドル)。
     */
    D3D12_CPU_DESCRIPTOR_HANDLE DsvCpuHandle() const noexcept;

    /**
     * 先頭 RTV の CPU ディスクリプタハンドルを返す (オフスクリーン RT のみの内部用)。
     *
     * @details 先頭スロット (slice0/mip0) の RTV を返す。
     * @return RTV の CPU ハンドル (RT でない / Device が無ければ null ハンドル)。
     */
    D3D12_CPU_DESCRIPTOR_HANDLE RtvCpuHandle() const noexcept;

    /**
     * 指定スライス・mip の per-slice RTV ハンドルを返す (内部用)。
     *
     * @details
     * per_slice_rtv=true で作成した cubemap 面 / 配列スライス / mip 用。スロットは
     * index = slice * mip_levels + mip の順で引く。
     * @param slice 配列スライス (cubemap 面) のインデックス。
     * @param mip mip レベルのインデックス。
     * @return per-slice RTV の CPU ハンドル (範囲外 / Device が無ければ null ハンドル)。
     */
    D3D12_CPU_DESCRIPTOR_HANDLE RtvCpuHandleForSlice(u32 slice, u32 mip) const noexcept;

    /**
     * 深度ターゲットとして作成されたかを返す。
     *
     * @return 深度ターゲットなら true。
     */
    bool                        IsDepth()       const noexcept { return m_IsDepth; }

    /**
     * 深度バッファが stencil 成分を持つかを返す (D24_UNorm_S8_UInt)。
     *
     * @details clear で stencil フラグを足すか、マスク描画で使えるかの判定に使う。
     * @return 深度かつ D24_UNorm_S8_UInt なら true。
     */
    bool                        HasStencil()    const noexcept {
        return m_IsDepth && m_Format == EFormat::D24_UNorm_S8_UInt;
    }

    /**
     * SRV を持つかを返す。
     *
     * @return SRV スロットが割り当て済みなら true。
     */
    bool                        HasSrv()        const noexcept { return m_SrvSlot >= 0; }

    /**
     * RTV を持つか (オフスクリーン RT か) を返す。
     *
     * @return RTV スロットが 1 つ以上あれば true。
     */
    bool                        HasRtv()        const noexcept { return !m_RtvSlots.IsEmpty(); }

    /**
     * MSAA サンプル数を返す (1=非 MSAA)。
     *
     * @return 作成時の sample_count (RT 以外は常に 1)。
     */
    u32                         SampleCount()   const noexcept { return m_SampleCount; }

    /**
     * 基となる D3D12 リソースを返す。
     *
     * @return ID3D12Resource ポインタ (未確保なら nullptr)。
     */
    ID3D12Resource*             Resource()      const noexcept { return m_Resource; }

    /**
     * 現在のリソース状態を返す。
     *
     * @return 直近に記録された D3D12_RESOURCE_STATES。
     */
    D3D12_RESOURCE_STATES CurrentState() const noexcept { return m_CurrentState; }

    /**
     * リソース状態を記録する (CommandList が状態遷移バリアを発行した後に呼んで反映する)。
     *
     * @param s 新しい D3D12_RESOURCE_STATES。
     */
    void SetCurrentState(D3D12_RESOURCE_STATES s) noexcept { m_CurrentState = s; }

private:
    /** 所有元の DX12 デバイス (ディスクリプタ解放に使う)。 */
    Dx12Device*           m_Device   = nullptr;

    /** 基となる D3D12 リソース。 */
    ID3D12Resource*       m_Resource = nullptr;

    /** テクスチャの幅 (ピクセル)。 */
    u32                   m_Width    = 0;

    /** テクスチャの高さ (ピクセル)。 */
    u32                   m_Height   = 0;

    /** ピクセルフォーマット。 */
    EFormat                m_Format   = EFormat::Unknown;

    /** mip レベル数 (最低 1)。 */
    u32                   m_MipLevels = 1;

    /** 配列スライス数 (最低 1、cubemap は 6 の倍数)。 */
    u32                   m_ArraySize = 1;

    /** MSAA サンプル数 (1=非 MSAA。RT 専用)。 */
    u32                   m_SampleCount = 1;

    /** キューブマップとして作成されたか。 */
    bool                  m_IsCubemap = false;

    /** SRV ディスクリプタヒープ内のスロット (未割り当ては -1)。 */
    i32                   m_SrvSlot = -1;

    /** DSV ディスクリプタヒープ内のスロット (未割り当ては -1)。 */
    i32                   m_DsvSlot = -1;

    /**
     * RTV スロット群。
     *
     * @details
     * 単一 RT は 1 個、per_slice_rtv は array_size*mip_levels 個
     * (index = slice*mip_levels + mip)。空なら RT ではない。
     */
    TArray<i32>           m_RtvSlots;

    /** 深度ターゲットとして作成されたか。 */
    bool                  m_IsDepth = false;

    /** 現在のリソース状態 (バリア追従用)。 */
    D3D12_RESOURCE_STATES m_CurrentState = D3D12_RESOURCE_STATE_COMMON;
};

} // namespace acs
