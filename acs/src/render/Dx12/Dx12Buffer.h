// SPDX-License-Identifier: Apache-2.0
// DX12 GPU バッファ実装
#pragma once

#include "render/IRhiBuffer.h"
#include "render/Dx12/Dx12Common.h"

namespace acs {

class Dx12Device;

/**
 * DX12 の GPU バッファ実装 (頂点・インデックス・定数バッファ用)。
 *
 * @details
 * cpu_writable=true のバッファは自動でフレームリング化され、kFramesInFlight 個の
 * スロットを 256B アラインのストライドで連続確保した UPLOAD ヒープリソースを永続マップする。
 * GPU が前フレームを読んでいる間に CPU が次フレーム分へ書き込めるよう衝突を避ける。
 * cpu_writable=false のバッファは DEFAULT ヒープに 1 スロットだけ確保する。
 */
class Dx12Buffer final : public IRhiBuffer {
public:
    /** 空状態で構築する (GPU リソースは Init で確保)。 */
    Dx12Buffer() noexcept = default;

    /** 永続マップを解除し GPU リソースを解放する。 */
    ~Dx12Buffer() noexcept override;

    /**
     * desc に従って GPU バッファリソースを確保する。
     *
     * @details
     * cpu_writable=true なら UPLOAD ヒープに kFramesInFlight スロット分を確保して永続マップし、
     * false なら DEFAULT ヒープに 1 スロット確保する。initial_data があれば全スロットへ複製する。
     * @param device リソース生成とフレームスロット問い合わせに使う DX12 デバイス。
     * @param desc サイズ・用途・cpu_writable・初期データを指定するバッファ記述。
     * @return 成功なら成功 HrResult、リソース生成・マップ失敗なら失敗 HrResult。
     */
    HrResult Init(Dx12Device& device, const FBufferDesc& desc) noexcept;

    /**
     * バッファの 1 フレームスロットあたりのバイト数を返す。
     *
     * @return Init で渡された desc.size。
     */
    usize       Size()  const noexcept override { return m_Size; }

    /**
     * バッファの用途を返す。
     *
     * @return Init で渡された EBufferUsage。
     */
    EBufferUsage Usage() const noexcept override { return m_Usage; }

    /**
     * CPU からデータを書き込む (cpu_writable=true のバッファのみ有効)。
     *
     * @details
     * フレームリング化されている場合は現在のフレームスロットの領域へ書き込む。
     * offset + size がバッファサイズを超える場合や未マップの場合は何もしない。
     * @param data 書き込むデータ先頭。
     * @param size 書き込むバイト数。
     * @param offset スロット先頭からの書き込みオフセット (既定 0)。
     */
    void Update(const void* data, usize size, usize offset = 0) noexcept override;

    /**
     * 内部の D3D12 リソースを返す (ビュー作成などの内部用)。
     *
     * @return GPU リソースへのポインタ。
     */
    ID3D12Resource*           Resource() const noexcept { return m_Resource; }

    /**
     * 現在の GPU 仮想アドレスを返す。
     *
     * @details フレームリング化されている場合は現在フレームスロットのオフセットを加算する。
     * @return GPU 仮想アドレス (リソース未確保なら 0)。
     */
    D3D12_GPU_VIRTUAL_ADDRESS Gpu()      const noexcept;

private:
    /** 永続マップと GPU リソースを解放し、再初期化可能な空状態へ戻す。 */
    void Reset() noexcept;

    /** フレームスロット問い合わせに使う DX12 デバイス。 */
    Dx12Device*     m_Device       = nullptr;

    /** 確保した D3D12 バッファリソース。 */
    ID3D12Resource* m_Resource     = nullptr;

    /** cpu_writable=true のときの永続マップ済み CPU ポインタ。 */
    void*           m_Mapped       = nullptr;

    /** 1 フレームスロットあたりのバイト数 (= desc.size)。 */
    usize           m_Size         = 0;

    /** フレームリング用のスロットストライド (256B アライン、リング無効なら 0)。 */
    usize           m_SlotStride  = 0;

    /** バッファの用途。 */
    EBufferUsage     m_Usage        = EBufferUsage::Vertex;

    /** CPU 書き込み可フラグ (true なら UPLOAD ヒープ + 永続マップ)。 */
    bool            m_bCpuWritable = false;

    /** フレームリング化フラグ (cpu_writable=true で自動 ON)。 */
    bool            m_bFrameCycled = false;
};

} // namespace acs
