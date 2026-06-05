// SPDX-License-Identifier: Apache-2.0
// DX12 グラフィックスパイプライン実装（PSO + RootSignature）
#pragma once

#include "render/IRhiPipeline.h"
#include "render/Dx12/Dx12Common.h"

namespace acs {

class Dx12Device;

/**
 * DX12 のグラフィックスパイプライン実装 (PSO + RootSignature)。
 *
 * @details
 * FPipelineDesc から、定数バッファ用の root CBV・テクスチャ用 descriptor table・
 * 静的サンプラを持つルートシグネチャと、ラスタライザ/ブレンド/深度ステンシル/
 * 入力レイアウトを設定した PSO を構築して所有する。
 */
class Dx12Pipeline final : public IRhiPipeline {
public:
    /** 空状態で構築する (PSO・RootSignature は Init で生成)。 */
    Dx12Pipeline() noexcept = default;

    /** PSO と RootSignature を解放する。 */
    ~Dx12Pipeline() noexcept override;

    /**
     * desc に従ってルートシグネチャ・入力レイアウト・PSO を構築する。
     *
     * @details
     * cbuffer_slots 個の root CBV と texture_slots 個の SRV descriptor table、
     * 最大 4 個の静的サンプラを持つルートシグネチャを生成し、ps が null なら
     * depth-only パイプラインとして PSO を作る。
     * @param device PSO・ルートシグネチャ生成に使う DX12 デバイス。
     * @param desc シェーダ・入力レイアウト・各種ステートを指定するパイプライン記述。
     * @return 成功なら成功 HrResult、引数不正や生成失敗なら失敗 HrResult。
     */
    HrResult Init(Dx12Device& device, const FPipelineDesc& desc) noexcept;

    /**
     * 構築済みの PSO を返す。
     *
     * @return パイプラインステートオブジェクト。
     */
    ID3D12PipelineState*   Pso()           const noexcept { return m_Pso; }

    /**
     * 構築済みのルートシグネチャを返す。
     *
     * @return ルートシグネチャ。
     */
    ID3D12RootSignature*   RootSignature() const noexcept { return m_RootSig; }

    /**
     * プリミティブトポロジを返す。
     *
     * @return Init で渡された EPrimitiveTopology。
     */
    EPrimitiveTopology      Topology()      const noexcept { return m_Topology; }

    /**
     * 定数バッファのスロット数を返す。
     *
     * @return root CBV のスロット数 (b0..)。
     */
    u32                    CBufferSlots()  const noexcept { return m_CbufferSlots; }

    /**
     * テクスチャのスロット数を返す。
     *
     * @return SRV descriptor table のスロット数 (t0..)。
     */
    u32                    TextureSlots()  const noexcept { return m_TextureSlots; }

private:
    /** 構築済みの PSO。 */
    ID3D12PipelineState* m_Pso      = nullptr;

    /** 構築済みのルートシグネチャ。 */
    ID3D12RootSignature* m_RootSig = nullptr;

    /** プリミティブトポロジ。 */
    EPrimitiveTopology    m_Topology = EPrimitiveTopology::TriangleList;

    /** 定数バッファのスロット数 (root CBV、b0..)。 */
    u32                  m_CbufferSlots = 0;

    /** テクスチャのスロット数 (SRV、t0..)。 */
    u32                  m_TextureSlots = 0;
};

} // namespace acs
